# CreateChatCompletionRequestAllOfPrediction

Configuration for a [Predicted Output](/docs/guides/predicted-outputs), which can greatly improve response times when large parts of the model response are known ahead of time. This is most common when you are regenerating a file with only minor changes to most of the content. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the predicted content you want to provide. This type is currently always &#x60;content&#x60;.  | 
**content** | [**PredictionContentContent**](PredictionContentContent.md) |  | 

## Example

```python
from openapi_client.models.create_chat_completion_request_all_of_prediction import CreateChatCompletionRequestAllOfPrediction

# TODO update the JSON string below
json = "{}"
# create an instance of CreateChatCompletionRequestAllOfPrediction from a JSON string
create_chat_completion_request_all_of_prediction_instance = CreateChatCompletionRequestAllOfPrediction.from_json(json)
# print the JSON string representation of the object
print(CreateChatCompletionRequestAllOfPrediction.to_json())

# convert the object into a dict
create_chat_completion_request_all_of_prediction_dict = create_chat_completion_request_all_of_prediction_instance.to_dict()
# create an instance of CreateChatCompletionRequestAllOfPrediction from a dict
create_chat_completion_request_all_of_prediction_from_dict = CreateChatCompletionRequestAllOfPrediction.from_dict(create_chat_completion_request_all_of_prediction_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


